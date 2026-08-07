# Session 2026-08-07/08 — the CEIR pivot + the CEIR-0 design phase (complete)

**Focus:** the user directed a major architectural pivot — **CEIR (the Cerid Execution IR) becomes the master spine
of D-007** — and we ran the entire CEIR-0 design phase to completion. All work is UNCOMMITTED at session end (user
commits; no AI co-author trailer). Mission constitution: `docs/research/2026-08-07-ceir-universal-programming-master-roadmap.md`.

## 1. The pivot

The gap the user named: *"we did not have a way to represent algorithms and techniques as assets."* RAF made the
render *schedule* an asset, but algorithms (renderers, culling, GI, tensor/ML graphs, UI effects) were still bespoke
C++. CEIR fixes this: **every reusable algorithm becomes a versioned, inspectable, serializable, hot-reloadable
program asset**; native C++ only for genuinely new host/hardware capability. Mantra: *ALGORITHMS ARE PROGRAM ASSETS ·
CAPABILITIES ARE NATIVE PRIMITIVES · COMPILERS CHOOSE LOWERINGS · BACKENDS EXECUTE.*

Locked directions (user): CEIR = the master spine (the old post-RAF 4-track table re-hangs under CEIR bands); **strict
band order** CEIR-0→32; **RAH runs in parallel**; everything else paused (§176). CHIR (the high-level language layer)
is design-only until CEIR-29 — the user accepted a **CHIR-0 design-only** concession rather than building it now.

## 2. Documents created

- **`docs/detours/D-007-ceir-tracker.md`** — THE new live tracker: an "architecture in one screen" primer + a jump
  table (concept → mission §§) + every sub-slice as a tick row with `→ §§` citations + a per-band contract block
  (types/seams/reuse/scars/gate). CEIR-0…13 fully decomposed; 14…32 one gated row each; a Paused table.
- **The mission** imported to `docs/research/2026-08-07-ceir-universal-programming-master-roadmap.md`.

## 3. CEIR-0 — the design phase (all ACCEPTED)

Every slice was drafted → advisor-reviewed → corrected → wired. Accepted by the user across 2026-08-07/08:

- **CEIR-0a** (`docs/design/ceir-0a-execution-path-inventory.md`) — the from-CODE inventory. **Headline finding: RAF
  already did the atomic-vs-composite split — CEIR is a promotion, not a rewrite.** The 14 executor verbs are atomic
  (promote to `ceir.render/compute/transfer/rt` op lowerings); the frame graph is the composite sequencer (→
  `ceir.frame`); `scene_renderer.cpp`'s 9-block `render()` + program hand-list is the real CEIR-13 migration target.
- **ADR-0108** (CEIR-0b) — owned language stack; **C++ is no longer the *only* authorable program.** Surgically
  supersedes ADR-0081 §9 ONLY (§1-§8 reaffirmed). The cornerstone flip (PRINCIPLES/AGENTS/README/ROADMAP) + the 0081
  §9 in-file strike are DEFERRED to the first CEIR vertical slice (CEIR-10z), so docs never claim a capability before
  it's real.
- **ADR-0109** (CEIR-0c) — the CEIR/CHIR/CKIR one-way layer contract + `crd-ceir` **host-only** module (deps
  core/log/memory/containers/units) + **dependency-inversion bridges** `crd-ceir-host`/`crd-ceir-gpu` (where the
  CEIR-0a `record_*` + CKIR-compile STAY) + invariants **I3/I4/I5** + the finalized CEIR-1 type names + the
  semantic-identity model. **Binding for CEIR-1.**
- **ADR-0110** (CEIR-0d) — an intrinsic = an ordinary CEIR-2 op + §100 native-binding metadata + a bridge handler.
  The legitimacy IFF test (capability=intrinsic · algorithm=program · **composable-but-slow=provider, never Level C**)
  + plugin levels A/B/C. Classifies the CEIR-0a atomic set.
- **CEIR-0e** (`ceir-0e-chir-0-language-design-note.md`) — CHIR-0 language design (design-only). Ownership leaning:
  **values + generational handles + arenas + state stores + a LIGHT borrow (NOT a full checker)**, tiebreak =
  visual+agent authorability. All binding decisions deferred to CEIR-29.
- **CEIR-0g** (`ceir-0g-maturity-and-manifest.md`) — the two ladders measure DIFFERENT axes → a **two-axis** model
  (`raf_level` today + `ceir_level` forward); §174 manifest gains `providers` + `determinism_tier`.
- **CEIR-0h** (`ceir-0h-migration-and-deletion-tables.md`) — the deletion ledger built from 0a: mostly PROMOTES;
  deletes = the composite C++ orchestration + duplicate/privileged paths, each with its parity gate named FIRST
  (F1–3 @ CEIR-12 · E1–5 @ CEIR-13 · R1–2 @ CEIR-11 · §PR-3/ADR-0106 supersessions). CEIR-31 executes it.
- **CEIR-0z** (`ceir-0z-close-report-and-sizing.md`) — the §184 fifteen-item report + honest DERIVED sizing
  (in-tree anchors): **CEIR-1…13 ≈ 34–55 KLOC**, **~4–8-month "dark period"** (very-low confidence, banded); 5
  design questions docketed to their bands.
- **CEIR-0f** — the D-007 restructure (this is the doc-surgery slice): a CEIR-spine section added to
  `D-007-gpu-program-system.md`; the old master table re-hung under CEIR bands + marked "do NOT tick live"; the live
  tracker wired (context.md + ROADMAP + detours/README moved to the CEIR tracker); the ADR-0106 supersession PLAN
  written (strike executes @ CEIR-12f); §PR-3's maturity ladder struck-in-place → §173 + the registry header.
  Additive: D-007 +41 lines, zero content deleted.

## 4. Process notes

- Every slice was advisor-reviewed before wiring — it caught real issues each time (the `visbuffer.raster`
  residual-special-case honesty, the units-dep earning, the `Operation`-vs-`Op` rename sweep, the two-axis maturity
  category error for T-class, the ADR-0106-strike-timing in the close report, three tense/status slips in the CHIR
  note — the aspirational-docs class the same-week hygiene pass had purged).
- The scripting-reversal acceptance honored §5's TWO gates: accepted now, but the PRINCIPLES/AGENTS/README/ROADMAP
  cornerstone flip is pinned to the first CEIR vertical slice (CEIR-10z) so the repo never carries a reversed
  cornerstone before the capability exists.

## 5. Next

- **Commit** the CEIR-0 batch (proposed messages below), then the two fronts open, both unblocked:
  - **CEIR-1a** — opens `engine/ceir` (the first code; ADR-0109 is its binding contract). Gated only on ADR-0109 (✅).
  - **RAH-1a.2** — the parallel front (delete the legacy G-buffer mechanic). Which front moves first is the user's call.

## 6. Proposed commits (user commits; NO AI co-author trailer)

1. `docs(ceir): CEIR-0 design phase — inventory, ADRs 0108/0109/0110, design notes (accepted)` —
   `docs/design/ceir-0{a,e,g,h,z}-*.md`, `docs/decisions/010{8,9}-*.md`, `docs/decisions/0110-*.md`,
   `docs/decisions/README.md`, `docs/design/README.md`, `docs/detours/D-007-ceir-tracker.md`.
2. `docs(ceir): CEIR-0f — restructure D-007 under the CEIR spine; live tracker moved` —
   `docs/detours/D-007-gpu-program-system.md`, `context.md`, `docs/ROADMAP.md`, `docs/detours/README.md`,
   `docs/capabilities/gpu-platform-capabilities.toml`, this session log.
