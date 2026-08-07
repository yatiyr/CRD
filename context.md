# Cerid — Live Context

> Short-term memory: "where are we now?" The master plan lives in `docs/ROADMAP.md`; the doc map in `docs/README.md`.
> This is a **DASHBOARD, not a changelog.** Each milestone's detail lives in its session log (`docs/sessions/YYYY-MM-DD-*.md`); this file summarises the *current* state and points there. Keep it lean (≤ 300 lines) — prune stale snapshots, don't stack them. (History pruned 2026-08-07 → `docs/sessions/2026-08-07-context-md-history-archive.md`.)

---

## Current focus — Post-RAF platform build across 4 parallel tracks (D-007)

**THE PLAN + LIVE STATUS LIVE IN `docs/detours/D-007-gpu-program-system.md`.** RAF (asset-driven render foundation) is
COMPLETE. The forward roadmap = **§POST-RAF PROGRAMME** + **§UI/2D SUB-PROGRAMME** there. **The master subslice table
(top of D-007) is the SINGLE SOURCE OF TRUTH** — it tracks **all 122 post-RAF sub-slices** as `✅/◧/⬜` rows in **four
parallel tracks**:

- **A — 3D rendering:** RAH→RPL→GVA→LSH→ARG→RTX→MAT→TPR→VFX→TXS→VGE
- **B — UI/2D:** I2D-0…9/PQ + SPR-0…4  (parked on RAH-1/2)
- **C — compute/science/ML:** CGP→HGP→MLR
- **D — platform/media/editor/qual/physics:** MED→D7E→PQP→EYL

Band contracts+DoD: §PR-7 (A/C/D) + §U-14/15/16 (B). Maturity L0–L7 + classes A/A+R/A+E/B/T + 18 invariants: §PR-3/4/5.
Per-feature machine-readable status: `docs/capabilities/gpu-platform-capabilities.toml`. ⛔ **A row ticks ✅ ONLY at its
slice GATE (L4/L5+, both backends where GPU) — NEVER because a shader or a completed "head" exists; nothing exceeds L5
today (L6 needs CR-D007/D7E).**

**HOW WE WORK (cadence, user-directed 2026-08-07):** the 4 tracks run in PARALLEL; a track STOPS when it depends on
unfinished work. **RAH is the shared root** — Tracks A & B's first *pipeline* slices need **RAH-1 (typed attachments) +
RAH-2 (resource-table bindless)** first. ⛔⛔ **Critical-path/foundational work (the command model, RAH-1/2) is done
DIRECTLY, never delegated.** ⛔⛔ **Implementation forks require `isolation:"worktree"` + a tight mandate + an explicit
"do NOT touch memory / D-007 / ADRs / CMake" clause** (see [[feedback_implementation_forks_need_worktree_isolation]]).
**User controls commits (no AI trailer).**

## Active track state

- **A / RAH-1 ◧** — ✅ **RAH-1a.1 (visbuffer fold) DONE + gated:** `RenderingDesc.visbuffer`/`clear_id` DELETED; visibility
  is a typed `ColorAttachmentDesc` (`clear_kind=Uint`+`clear_uint`); REN-38-F6 PASS both backends (97 asserts).
  **NEXT = RAH-1a.2 (approach = DELETE, user-chosen):** retire `IGBufferTarget`+`draw_gbuffer`+`create_gbuffer_target`
  (both backends) + `RenderingDesc.gbuffer`, migrate ~8 test sites to the `color`-span MRT path (live deferred already
  uses `color1..3`); needs a plain-vertex-MRT-color-span path + regular-target readback first. Exact site list is in
  the RAH-1 row. Then **RAH-2**.
- **B — parked** on RAH-1/2 + the ADR-0107 review. No I2D/SPR code until both.
- **C / CGP-0 ◧ + CUDA ✅** — portable `IComputeContext::last_gpu_ms()` + DX12 timestamps (Vk+DX12 gated); **CUDA is a
  third `IComputeContext` backend** (`engine/gpu-context-cuda`, reuses `kir-cuda`, gated on the real 4070 Ti — 11 asserts;
  the user's CUDA directive, [[feedback_cuda_is_a_required_gpu_compute_backend]]). NEXT: `create_best_compute_context`
  selector (CUDA>Vk>DX12) + capability queries.
- **D / MED-1 ◧** — GIF single-frame decode + engine's first LZW (`engine/resources`, 778 asserts). NEXT: animated GIF →
  TIFF → progressive JPEG. Follow-up owed: a real-GIF external-oracle corpus (encoder+decoder both ours — the
  bit-exact-symmetric-bug scar).

## Recently landed

- **2026-08-07 (later)** — repository-wide **documentation hygiene pass** (uncommitted): context.md → dashboard
  (history archived), ROADMAP/systems/debt/AGENTS/READMEs refreshed to honest state, retired-module overviews
  DELETED (user direction; git history keeps them), research outcome stamps, ADR index + link fixes. Full report:
  `docs/sessions/2026-08-07-doc-hygiene-pass.md`.
- **2026-08-07** — post-RAF 4-track kickoff: RAH-1a.1 + CGP-0/CUDA + MED-1 (`c116e98`); D-007 §POST-RAF + §UI/2D
  programmes + four-track tracker (`e3f8e5e`). Log: `docs/sessions/2026-08-07-post-raf-tracks-rah1-cuda.md`.
- **2026-08-06** — **RAF band COMPLETE** (`af3e04c`): `FramePassKind` retired, ADR-0106 closed.
  Log: `docs/sessions/2026-08-06-raf12-3-retire-framepasskind.md`.
- **2026-08-03…05** — RAF-0…12: substrate → one-submission frame graph → executors → engine-default assets →
  app-custom renderer → hot reload → legacy deletion. Logs: `docs/sessions/2026-08-0{3,4,5}-raf*.md`.

## Open questions / risks

- **Pending user review:** RAH-0 audit (`docs/systems/rah-0-canonical-model-audit.md`) + ADR-0107
  (`docs/decisions/0107-ui-2d-architecture.md`). Track B code is blocked on the ADR-0107 review.
- `MEMORY.md` ≈ 19.9 KB (hard read limit 24.4 KB) — deeper cull deferred, entries must be MERGED/DROPPED not just
  hook-trimmed.
- The integrated CUDA fork worktree `.claude/worktrees/agent-af34b487c5544c8fa` can be removed.

## Gates that matter

Per-slice DoD: `scripts/per-slice-check.ps1` (+ `-IncludeRelease` for GPU/LTCG slices); cluster close =
`scripts/full-sweep.ps1` (18-config). **Run `ctest`, never the bare test binary** (guards are ctest-only). GPU slices:
`ValidationCapture` + both backends. Tidy per touched file via `scripts/tidy-files.ps1`, never accumulated.

## Active detour

**D-007 (merged with D-008 on 2026-07-11) — the GPU program system.** ACTIVE; grew out of hesap v17 (GPU compute,
kicked off 2026-07-07). RAF + the whole post-RAF programme run under it. Everything above is D-007 state. Queue rules:
`docs/detours/README.md`.

## Recent milestones (one line each; details in session logs + `docs/bench/`)

- **2026-08-06 — RAF complete:** engine renderers are ordinary assets; one backend-neutral command model; executor
  registry; hot reload; legacy paths deleted (ADR-0106).
- **2026-07-21…08-03 — REN-36…41:** authored frame graphs/techniques/materials (`.crdm/.crdt/.crdv/.crdl/.frame.toml`),
  bindless+multi-draw (38-G1 119 fps), indexed-pull reuse, O(chunks) extract, soft shadows (PCSS/EVSM/MSM), velocity +
  TAA, Nanite-class cluster LOD start.
- **2026-07-13…16 — the GPU compute crush campaigns:** 2D FFT 1.16–1.20× cuFFT bit-exact; reduction beats CUB; radix
  sort 0.73× CUB (bit-exact, 8.4× session gain); NRC fused MLP 2.37× cuBLAS; B14 SVGF/DDGI/ReSTIR/NRC + B15
  atmosphere/clouds + B16 FFT ocean — all gold-standard CKIR. (Narrative: the context-history archive; boards:
  `docs/bench/`.)
- **2026-07-10…12 — D-007 device+IR convergence:** one `VkDevice`, I1/I2 leak gates closed, oracle rounds per-op, CUDA
  fan-out bit-exact.
- **2026-07-23 — RET band: crd-rhi/rhi-vulkan/renderer/shader DELETED** (ADR-0105); gpu-context IS the graphics layer.
- **2026-07-02 — hesap v13 close:** interpolation/quadrature/differentiation/motion — full peer-board crush (scipy/
  MATLAB/Boost/GSL/Ruckig).
- **Earlier (hesap v0→v12, geometry, units, scene/ECS):** see `docs/phases/` + the archive.

## Paused main-roadmap work

- **Phase 3.1.6 hesap:** paused mid-v17 (GPU compute) — v17's substrate is being built AS D-007; hesap-GPU is the
  detour's last stop. v14 tensors ✅ (2026-07-05) · v15 forward AD ✅ · v16 reverse AD ✅ (2026-07-07, ADR-0097).
  `docs/phases/phase-3.1.6-hesap.md`.
- **Phase 3.1 eylem:** ⏸ paused at v1b close (ADR-0076 §12 sequencing); resumes v1c+ after the detour + hesap.
  `docs/phases/phase-3.1-eylem.md`.

For the full doc map: `docs/README.md`. ADR index: `docs/decisions/README.md`. Open debt: `docs/debt.md`.
