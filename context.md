# Cerid — Live Context

> Short-term memory: "where are we now?" The master plan lives in `docs/ROADMAP.md`; the doc map in `docs/README.md`.
> This is a **DASHBOARD, not a changelog.** Each milestone's detail lives in its session log (`docs/sessions/YYYY-MM-DD-*.md`); this file summarises the *current* state and points there. Keep it lean (≤ 300 lines) — prune stale snapshots, don't stack them. (History pruned 2026-08-07 → `docs/sessions/2026-08-07-context-md-history-archive.md`.)

---

## Current focus — CEIR is the master spine of D-007 (design phase DONE; CEIR-1 next)

**⛔ THE LIVE TRACKER IS `docs/detours/D-007-ceir-tracker.md`** (CEIR bands 0–32 + the RAH parallel track). CEIR — the
Cerid Execution IR — is the new master architectural spine (user-directed 2026-08-07): every reusable algorithm becomes
a versioned, inspectable, hot-reloadable **program asset**; native C++ only for genuinely new host/hardware capability.
**THE LAW:** `docs/research/2026-08-07-ceir-universal-programming-master-roadmap.md` (§0–§185). Mantra: *ALGORITHMS ARE
PROGRAM ASSETS · CAPABILITIES ARE NATIVE PRIMITIVES · COMPILERS CHOOSE LOWERINGS · BACKENDS EXECUTE.* The old post-RAF
4-track table in `D-007-gpu-program-system.md` is **re-hung under the CEIR bands** and preserved as history/contracts
(A/RPL→CEIR-15 · C/MLR→CEIR-21 · hesap-GPU→CEIR-19 · frame+executor→CEIR-12/13 · B/I2D→CEIR-28 · D/D7E→CEIR-30).

**CEIR-0 DESIGN PHASE COMPLETE + ACCEPTED (2026-08-07/08):** 0a inventory (headline: RAF already did the atomic-vs-
composite split → CEIR is a promotion, not a rewrite) · **ADR-0108** (owned language stack; C++ no longer the *only*
authorable program — supersedes ADR-0081 §9) · **ADR-0109** (CEIR/CHIR/CKIR one-way layer contract + `crd-ceir`
host-only module + `crd-ceir-host`/`crd-ceir-gpu` dependency-inversion bridges + I3/I4/I5; **binding for CEIR-1**) ·
**ADR-0110** (native-intrinsic schema + plugin levels) · 0e CHIR-0 note · 0g two-axis maturity model · 0h deletion
ledger · 0z §184 report + sizing (CEIR-1…13 ≈ 34–55 KLOC, ~4–8 mo). CEIR-0f (D-007 restructure) executing.

**HOW WE WORK (user-directed):** **strict band order** CEIR-0→32; each band closes at its gate before the next. **RAH
runs in PARALLEL** (the binding/attachment vocabulary CEIR-9/11 lower onto). ⛔ Everything else PAUSED (§176: only bug
fixes, CKIR fixes, tests, docs, RAH, CEIR). ⛔⛔ Foundational/critical-path work done DIRECTLY, never delegated. ⛔⛔
Implementation forks require `isolation:"worktree"` + a tight mandate ([[feedback_implementation_forks_need_worktree_isolation]]).
**User controls commits (no AI trailer).**

## Active state

- **CEIR (spine) — design phase ✅; CEIR-1a is the first line of code**, gated only on ADR-0109 (✅ Accepted). After the
  accepted 0a–0z batch is committed + CEIR-0f lands: open **CEIR-1a** (`crd::ceir::{Context,Module,Operation,Value,
  Block,Region}` with arena storage). Band order + per-slice contracts + §§: the CEIR tracker.
- **RAH (parallel track) — front = RAH-1a.2.** ✅ RAH-1a.1 (visbuffer fold) DONE + gated (REN-38-F6, 97 asserts, both
  backends). **NEXT = RAH-1a.2 (DELETE, user-chosen):** retire `IGBufferTarget`+`draw_gbuffer`+`create_gbuffer_target`
  (both backends) + `RenderingDesc.gbuffer`; migrate ~8 test sites to the `color`-span MRT path; needs a
  plain-vertex-MRT-color-span path + regular-target readback first. Then RAH-1a-close → RAH-2 (unblocks CEIR-11/B).
- **PAUSED (parked, not dropped):** B/I2D+SPR (ADR-0107 review pending) · C/CGP selector + HGP/MLR · D/MED codecs
  (animated GIF→TIFF→JPEG; real-GIF external-oracle corpus owed) · main roadmap (hesap v18, eylem v1c+). Resume paths:
  the CEIR tracker's "Paused" table.

## Recently landed

- **2026-08-07 (later)** — repository-wide **documentation hygiene pass** (uncommitted): context.md → dashboard
  (history archived), ROADMAP/systems/debt/AGENTS/READMEs refreshed to honest state, retired-module overviews
  DELETED (user direction; git history keeps them), research outcome stamps, ADR index + link fixes. Full report:
  `docs/sessions/2026-08-07-doc-hygiene-pass.md`.
- **2026-08-07/08** — **CEIR pivot + CEIR-0 design phase COMPLETE:** CEIR becomes the master spine; the live tracker
  `docs/detours/D-007-ceir-tracker.md` created; CEIR-0a inventory + ADRs 0108/0109/0110 + the 0e/0g/0h/0z design notes
  all accepted; D-007 restructured (CEIR-0f). (uncommitted at time of writing — user commits.)
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

**D-007 (merged with D-008 on 2026-07-11) — the GPU program system.** ACTIVE; grew out of hesap v17 (2026-07-07).
CKIR IR + gpu-context convergence + the full visual frontier + RAF (all ✅) → now **CEIR is the master spine** (2026-08-07;
the live tracker is `docs/detours/D-007-ceir-tracker.md`, the landed-history ledger is `D-007-gpu-program-system.md`).
Everything above is D-007 state. Queue rules: `docs/detours/README.md`.

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
