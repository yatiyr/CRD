# CEIR-34 — Legacy-deletion census (band-open, GATE-re-verifies)

<!-- doc-role: reference -->
> Technical reference; verify dated claims against current contracts/source. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

> 🔴🔴 **SUPERSEDED IN PART by the user ruling 2026-09-11 (recorded in `context.md`).** This census RECOMMENDED
> parking E4 (§3 Decision A2) and R2 (§3 Decision B2). The user OVERRODE that: **pull E4 + R2 into the loop as REAL
> deletions** (no park). Both are now **DONE** — E4: the imperative `register_default_programs` hand-list → the
> `assets/scene_programs.manifest` + `register_from_manifest` loader (4 configs + tidy); R2: the
> `draw_overlay`/`draw_overlay_range`/`record_overlay` device verb RETIRED both backends (the overlay rides generic
> `draw_storage`/`draw_storage_depth_load`) + a driven-out pre-existing DX12 `first_vertex`→`SV_VertexID` defect fixed
> via a backend identity index buffer (4 configs + tidy) — log:
> `docs/sessions/2026-09-11-ceir-34-r2-overlay-verb-retirement.md`. Also SUPERSEDED: the line-9 "CEIR-33 parked for
> I2D" — CEIR-33 is UN-PARKED (driven after 34/35 in this loop). The §2 census GRADES (every other 0h row
> DONE/SATISFIED) STAND and were re-verified at 34a; only the §3/§4 park recommendations are overridden.

> **Band:** D-007 · CEIR-34 · slice 34-0 (census). **Tracker row:** `docs/detours/D-007-ceir-tracker.md` → CEIR-34.
> **Contract (§175 band-34 · §178):** *Legacy deletion → **one execution-program architecture**; every CEIR-0h row executed.*
> **Grades against:** the **CEIR-0h deletion ledger** (`docs/design/ceir-0h-migration-and-deletion-tables.md`). The
> ledger's own header names its law — *"Law: mission §126 (framegraph migration), §127 (executor migration), §178
> (must-not-stay-native)"* — so the ledger's F/E/R/M tables ARE the §178 checklist; there is no separate §178 prose in
> the mission doc (searched). **Opened:** 2026-09-11 (autonomous CEIR loop; user authorized driving 34→35).
> ⛔ **CEIR-33 is DELIBERATELY PARKED for I2D** (user direction 2026-09-11); this band closes the CEIR loop on 34 → 35.

---

## 1. The rule this band enforces (from CEIR-0h §1)

Every 0h row is a thing that gets DELETED, and **its parity gate is named before the deletion** — the deletion is
proven, not asserted. ⛔ A deletion with no green parity gate is forbidden. This band is **census-first** (the
GATE-re-verifies-rows doctrine): walk every row, confirm executed with file:line evidence, and delete ONLY genuine
residual. ⛔ No over-deletion — the 0h ledger + §178 bound the deletion set; nothing beyond them is invented to make
the band feel substantial.

## 2. Census — every 0h row graded with evidence

Status legend: **DONE** = deleted at a prior band, gate green · **SATISFIED** = the ledger's target is met by promotion
(the C++ became authored CEIR / a sanctioned host resolver), no further deletion owed · **OPEN** = residual remains =
a decision or a deletion this band must resolve.

### Frame-path (CEIR-0h §3)

| # | Target | Status | Evidence |
|---|---|---|---|
| F1 | private frame-blob path / `frame_runtime.cpp` as a *distinct* driver — folded into the CEIR plan path | **SATISFIED** | `frame_runtime.cpp:1` "REN-36.2: drive `IFrameGraph` from a cooked `FrameGraphDesc`"; it now feeds the CEIR plan path (`#include <crd/ceir/gpu/render_fullscreen_build.hpp>`, `build_frame_plans`, CEIR-16-3c). It is the cook→CEIR **frontend**, not a distinct driver (0h §8 item 1 disposition). |
| F2 | `FrameGraphTemplate::add_pass` construct-and-execute as a privileged bypass (§121) | **DONE** | CEIR-15f/15z: `CRD_FRAME_VIA_CEIR` flag + `=0` bypass DELETED; `validate_ceir_frame` is the live cook gate (tracker CEIR-15z, line ~650). |
| F3 | ADR-0106 "render-graph is THE runtime" as a standalone claim — struck in place | **DONE** | Struck at CEIR-15f; render-graph is the `crd-ceir-gpu` provider's execution of CEIR plans (tracker CEIR-15z). |

### Executor / orchestration (CEIR-0h §4)

| # | Target | Status | Evidence |
|---|---|---|---|
| E1 | program-variant selection matrix (the hand-coded permutation ladder) | **DONE** | CEIR-16d §128: `record_scene_raster` DELETED, `scene.raster` maps to `record_ceir_render` UNCONDITIONALLY (tracker CEIR-16d-live-4c, line 677). |
| E2 | `render()` frame-graph assembly + cull + draw-list blocks → authored `ceir.frame` + resolvers | **DONE** | CEIR-16d + CEIR-17: the composite executors record through the generic path; scene bridge proved (17a-e). |
| E3a | cascade-fit + shadow **orchestration** C++ → a host resolver for the fit + a `ceir.frame` shadow sub-graph | **SATISFIED** | 17c proved the CEIR resolve chain reproduces the twin selection (`frame_runtime.cpp:192-193`); the shadow orchestration is the authored `forward_csm` frame graph; **the camera-fit math surviving as a host resolver is EXPLICITLY PERMITTED by the ledger** ("a host resolver for the camera fit"). 17z: band 17 was PROMOTION not deletion — imperative deletion was §128/CEIR-16. ⛔ Not an over-delete target. |
| E3b | gold shadow-corpus breadth (§88 PCSS/EVSM/MSM/VSM/contact) as CEIR shadow-technique assets | **DONE (verify)** | CEIR-15 shadow corpus (§88); CEIR-15 CLOSED (tracker line 650). Final verify: each §88 variant is a committed asset — owed as a re-verification check, not a deletion. |
| E4 | the program **hand-list** (`init_programs` / `register_default_programs`) → the **RAH-7** dependency-driven registry | **⛔ OPEN — USER DECISION** | `register_default_programs()` ALIVE at `scene_renderer.cpp:2747`; `init_programs` is the live RAF-9/10/11 registry population. The 0h gate says "hand-list DELETED at **RAH-7**" — but **RAH-7 was collapsed at the 2026-08-09 re-baseline; the RAH band now ends at RAH-2** (tracker lines 141-147). So E4 has NO landing slice. **Fork below (§3).** |
| E5 | `SceneRenderer::render` as the C++ render path → an orchestrator that loads + executes the CEIR frame asset | **DONE** | Closed with E1/E2 at CEIR-16d (no private C++ render path; §128). |

### Residual special-cases (CEIR-0h §5)

| # | Target | Status | Evidence |
|---|---|---|---|
| R1 | `visbuffer.raster` executor → `scene.raster` into a uint attachment | **DONE** | CEIR-16z: `visbuffer.raster` dissolved (procedural geometry + uint clear); `record_visbuffer_raster` DELETED, 14→13 built-ins (tracker CEIR-16z, line 678). |
| R2 | `submit_overlay` composite + the `draw_overlay` **device verb** → a small `ceir.render` LOAD+blend+RO-depth program | **⛔ OPEN — USER DECISION** | `draw_overlay`/`draw_overlay_range` are STILL live device verbs in the canonical encoder (`command_lowering.hpp:311,315`, RAF-12.4); `submit_overlay` still exists (`overlay_pass.cpp:98`). **ImGui reaches the screen through this path** (`draw-imgui`→`add_draw_overlay_pass`→`submit_overlay`). Not yet re-authored as a CEIR program. **Fork below (§3).** |

### Supersessions (CEIR-0h §6 — struck-in-place, NOT deletions)

| # | Target | Status | Evidence |
|---|---|---|---|
| M1 | §PR-3 standalone maturity ladder → struck in place, pointed at §173 | **DONE** | CEIR-0f (tracker line 166). Not a deletion line; text preserved as history. |

### CEIR-0h §7 / §8 line-items

- **§7 (the §174 manifest `executors = [...]` must reference `ceir.*` ops, not dead RAF names):** **DONE (verify).** No
  `executors = [...]` assignment exists in `engine/` code (only a registration comment in `executor_registry.hpp:183`);
  consistent with the CEIR-13c migration having happened. Final verify: confirm no committed manifest carries dead RAF
  executor names.
- **§8 item 1 (`frame_runtime.cpp` deleted outright vs. retained as a thin frontend):** **RESOLVED — retained as the
  thin cook→CEIR frontend** (see F1). It is `M` in the working tree as ordinary CEIR-era edits, not F1 residue.

## 3. The two open items — both USER DECISIONS (the loop will NOT execute these autonomously)

Both are legacy deletions whose named gate/replacement is I2D/RAH-adjacent. Per the standing mandates, deletions are
irreversible and these two have couplings, so they are surfaced as decisions rather than driven blind.

### Decision A — E4 (the program hand-list)
- **State:** `register_default_programs`/`init_programs` alive; its 0h replacement (the RAH-7 dependency-driven registry)
  was collapsed at re-baseline — no landing slice exists.
- **Fork:** **(A1)** pull a RAH-7-class dependency-driven program registry into the CEIR loop and delete the hand-list
  against its enumerable set-equality gate; **(A2)** PARK E4 with RAH/I2D (the registry is I2D-substrate; RAH-2, its
  sibling, is itself an I2D gate per ADR-0107:119).
- **Recommendation: A2 (park).** The dependency-driven registry is a capability upgrade in I2D/RAH territory, not the
  execution-spine deletion CEIR-34 exists for; RAH-2 is already I2D-gated, so the registry naturally belongs there. The
  hand-list is not a *privileged/duplicate* path (§178's target) — it is the current registry's population, functionally
  correct. Parking it leaves no execution-architecture gap; it defers an ergonomics upgrade to where its gate lives.

### Decision B — R2 (the overlay device verb)
- **State:** `draw_overlay` is a live device verb; ImGui composites through it.
- **Fork:** **(B1)** re-author the overlay as a small `ceir.render` LOAD+blend+RO-depth program now (keeping ImGui
  working through the CEIR overlay — the gate: overlay renders identically); **(B2)** DEFER with the overlay verb until
  the I2D product UI lands (ADR-0107 §36 keeps ImGui until then).
- **Recommendation: B1 is doable and in-spirit, but low-value now.** The overlay verb is a *permanent special case* the
  ledger wants dissolved; re-authoring it as CEIR is the clean end-state and keeps ImGui working. But it touches the
  debug-UI path ADR-0107 §36 protects until I2D, so the risk/reward favors **B2 (defer with I2D)** unless the user wants
  the special-case gone now. Either way, ImGui is NOT deleted.

## 4. Conclusion — CEIR-34 is a re-verification band

Every 0h row is **DONE / SATISFIED** except **E4** and **R2**, both of which are **user decisions that lean toward
parking with I2D/RAH** (neither is a privileged/duplicate execution path that §178 forbids staying native; both are
functionally correct today, and their replacements live in I2D/RAH substrate). If the user rules "park both," CEIR-34
closes as a **re-verification band**: a fresh gate re-run of the DONE rows (the 17z discipline — deletion-gate re-run +
HEAD baseline caught 3 latent CEIR-replay drops that per-slice gates missed) plus the E3b/§7 verify checks, with E4/R2
recorded in the deferral ledger against their I2D/RAH triggers. That is a legitimate close — a census that finds every
spine deletion already executed is the correct outcome, not a band to pad.

**Remaining band work (34-0 → 34z):**
1. **34-0 (this doc):** census — DONE.
2. **User ruling on E4 (Decision A) + R2 (Decision B).**
3. **34a re-verification:** re-run the DONE-row gates (frame-cook + scene-render + render-graph + gpu-context both
   backends) + HEAD baseline; verify E3b §88 variants + §7 manifest names; record E4/R2 in the deferral ledger.
4. **34z band close:** flip the CEIR-34 row ✅, session log, context.md, proposed Conventional-Commits message.
