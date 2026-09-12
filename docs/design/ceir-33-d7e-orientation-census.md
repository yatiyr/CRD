# CEIR-33 (D7E — the universal program editor) — orientation census

<!-- doc-role: reference -->
> Technical reference; verify dated claims against current contracts/source. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

> **Status: ORIENTATION (2026-09-11).** CEIR-33 was un-parked into the autonomous loop with the
> user's model recorded as *"D7E is the editor that RIDES the finished substrate."* This census
> establishes what CEIR-33 actually decomposes into against the accepted architecture, grades the
> **drivable** half now, and surfaces the **gated** half as the same review the user already owns.
> It does not pick any fork. Mirrors `ceir-34-legacy-deletion-census.md` / `ceir-35-production-qualification-census.md`.

## 0. Verdict in one line

CEIR-33/D7E is **two halves**: (33a) the *domain contracts* the editors satisfy — substrate-side,
**drivable now**, and largely already delivered by the CEIR bands; and (33b) the *editor widgets
themselves* — which ADR-0107 D8 reframes as **I2D-9 flagship widgets on the CR-D007 shell (I2D-4)**,
i.e. the I2D UI programme, **not drivable** until ADR-0107 is accepted and the I2D-1…4 shell is built.
**Nobody authorized building a UI framework**; the "rides the finished substrate" model maps to 33a.

## 1. The architectural split (with citations)

- **ADR-0107 §D8** (verbatim): *"D7E is reframed, not duplicated: the post-RAF D7E band no longer
  builds its own UI. Its editor domains (frame-graph / material / CKIR / geometry graph editors; …
  inspectors; capture/regression/profiling) are delivered as **I2D-9 flagship widgets on the
  CR-D007 shell (I2D-4)**. D7E therefore = the *domain contracts* those widgets satisfy, and it
  defines the L6 bar."*
- **ADR-0107 status = "Proposed"** (pending user review) — the I2D-0 gate. The I2D bootstrap it
  sequences (Canvas MVP I2D-1 → Text MVP I2D-2 → UiWorld MVP I2D-3 → CR-D007 bootstrap I2D-4) has
  **no code in the tree** (grep: no I2D / CR-D007 sources). ImGui coexists (§36, kept for debug).
- **The L6 bar is the same blocker.** `docs/capabilities/gpu-platform-capabilities.toml` (the §174
  manifest) header: *"⚠ Nothing exceeds L5 today: L6 requires a CR-D007 inspection/authoring schema
  (band D7E, unstarted)."* So the §174 manifest L6 residual (Q10) and CEIR-33's UI half are **one
  blocker wearing two hats** — no feature reaches L6 until CR-D007 (I2D-4) lands.

## 2. 33a — the domain-contract inventory (the drivable slice, graded now)

For each D8 editor domain: does an **authorable asset schema** exist, with a **round-trip / cook
gate**, and a **hot-reload path**? Grades are contract-side only (the widget that renders them is 33b).

| D8 domain | Authorable schema | Round-trip / cook gate | Grade |
|---|---|---|---|
| frame-graph editor | `.frame.toml` — 23 assets (ADR-0127 `ceir.frame`) | `validate_ceir_frame` cook gate; `tests/assets/frame-cook/test_frame_ceir.cpp` | **DONE** |
| material editor | `.crdm` — 6 assets (MaterialX arg-order contract) | `tests/assets/material-cook/`, `tests/rendering/draw/test_ckir_draw_shaders.cpp` | **DONE** |
| CKIR editor | `.ckir` — 42 assets (⛔ authored direct, no C++ KGraph) | pipeline roundtrip `test_ceir_pipeline_vulkan.cpp`; 35b malformed-reject hardening | **DONE** |
| CEIR/CHIR program-graph editor | `.chirgraph`/`.ceir` — 17 assets (CEIR-32b schema) | printer/parser byte-stable; band gates `test_band*_gate.cpp` | **DONE** |
| geometry-graph editor | — (only `rt`/`scene` ops; no `ceir.geom` graph dialect) | — | **GAP** (demand-driven by I2D-9) |
| inspectors (frame/resource/program/material/hot-reload) | hot-reload = CEIR-10a decision table (`engine/execution/ceir-cook/…/hot_reload.hpp/.cpp`) | HotSwap/NoChange/NeedsMigration/ContractChange-reject | **PARTIAL** (hot-reload DONE; the rest are runtime introspection, not asset contracts) |
| capture / regression / profiling | test harnesses + RenderDoc tooling (`reference_renderdoc_headless_capture`), no D7E-consumable contract asset | (tooling exists; not a contract) | **GAP** (demand-driven) |

**Reading:** 4 DONE, 1 PARTIAL, 2 GAP. The four DONE domains are exactly the substrate the user's
"rides the finished substrate" model names — authoring + round-trip + cook + hot-reload already
exist. The GAP/PARTIAL items are **demand-driven by the I2D-9 widget** (D8 lists them as widget
domains): defining a `ceir.geom` graph schema or a capture/profiling contract *asset* **before the
widget that consumes it exists** would be speculative invention (⛔ search-engine-before-building;
⛔ everything-is-an-authorable-asset is a pull from a consumer, not a push). So they are correctly
GAP-until-consumed, not GAP-as-debt.

## 3. Decision C — the UI half (SURFACE, do not pick)

CEIR-33's UI half rides ADR-0107, which is **already pending your review**. This is not a new ask.

- **C1 — accept ADR-0107 → the I2D band proceeds** (I2D-1 Canvas → I2D-2 Text → I2D-3 UiWorld →
  I2D-4 CR-D007 → … → I2D-9 flagship widgets). CEIR-33/D7E **closes when I2D-9 closes**; L6 unlocks
  with CR-D007. This is a large multi-band UI programme, not a CEIR slice.
- **C2 — rule that CEIR-33's *deliverable* is the domain contracts + the L6 bar** (33a), and
  re-scope the widgets explicitly to I2D-9. CEIR-33 then closes on the contract inventory (§2)
  reaching DONE for the shipping domains, with the widgets tracked as I2D-9. **Leaning C2** — it is
  D8's own text ("D7E therefore = the domain contracts those widgets satisfy").

**✅ RESOLVED 2026-09-11 — C2 (ruled by the user).** The user selected "C2 contracts + L6 bar" directly. So **CEIR-33's
deliverable is the 33a domain-contract inventory (§2: 4 DONE / 1 PARTIAL / 2 GAP-until-consumed) + the L6 bar**, and the
flagship widgets are re-scoped explicitly to the **I2D-9** slice of the I2D UI programme (they ride ADR-0107 there, on their own
timeline). CEIR-33/D7E therefore **closes** on the contract inventory being DONE for the shipping domains — which it is — with
the widgets + the CEIR-L7 authoring ceiling tracked as I2D-9 (see the §174 manifest ceiling note). CEIR-33 is no longer an open
CEIR-loop item; its remaining half is an I2D-band obligation.

Under **either** fork, the drivable work is identical and it is 33a. C is the *closure* question — now answered C2.

## 4. Consolidated user-gated decisions (one place — see context.md §Gated)

CEIR-33 orientation adds Decision C to the loop's surfaced-and-waiting set. All four ride reviews
you already own; none blocks the drivable work:

- **Decision A** (CEIR-35) — whole-engine quality qualification (A1 pull-in-by-list vs A2
  close-and-route; leaning A2). Needs acceptance criteria + bench design.
- **Q7** (CEIR-35) — perf-board bench-design steer.
- **Q10 / §174 manifest** — the ceir-0g §4 two-axis migration (`schema=2`, `ceir_level`/`providers`/
  `determinism_tier`) + matrix generator. **Same L6 blocker as Decision C** (L6 = CR-D007), plus a
  judgment-heavy per-feature `ceir_level` classification — not a blind fill.
- **Decision C** (this census) — CEIR-33 closure fork; rides the **already-pending ADR-0107 review**.

## 5. What the loop does next (autonomous, no gate)

- 33a stays at **4 DONE / 1 PARTIAL / 2 GAP-until-consumed** — no speculative schema invention.
- The tracker CEIR-33 row + context.md record "two halves; 33a driving; widgets gated on the pending
  ADR-0107 review" (**not** "blocked" — the drivable half is done substrate).
- The loop continues on any remaining ungated substrate work; the four gated decisions wait for the
  user in one consolidated block.
