# CEIR-35z — the user-gated decisions (one place to cross the wall)

<!-- doc-role: reference -->
> Technical reference; verify dated claims against current contracts/source. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

> **Why this doc exists.** The autonomous CEIR loop has driven every *ungated* item to done (spine §7,
> breadth §8, docs §10, the §174 schema=2 manifest at 50 rows + generator + CI gates). What remains
> before **35z close** is **not drivable autonomously** — it needs your ruling. Those rulings were
> scattered across `context.md`, the CEIR-35 census §9, and the CEIR-33 orientation census §3/§4. This
> is the single packet: **three questions, each with the forks, my lean + why, what each fork unblocks,
> and the exact files/gates that change.** Where I *can* pre-draft the mechanical half of a fork (A2's
> routing table), it is drafted below so you pick, not build.
>
> **Nothing here blocks the loop's remaining drivable work — there is none left.** These are the close.
> Authoritative sources (this doc points, does not duplicate): CEIR-35 census
> `docs/design/ceir-35-production-qualification-census.md` §8/§9/§10 · CEIR-33 orientation census
> `docs/design/ceir-33-d7e-orientation-census.md` §2/§3 · `context.md` §Gated · `docs/detours/D-007-ceir-tracker.md`
> (CEIR-33/35 rows) · the §174 manifest `docs/capabilities/gpu-platform-capabilities.toml`.

---

## Decision A — does "CEIR-35 = whole-engine PQP" mean pull quality gates *in*, or close on the substrate and *route* quality to the feature bands?

**The question in one sentence.** You ruled "CEIR-35 = whole-engine PQP." The **(B) breadth
re-verification** — every engine family that rides the CEIR substrate runs green under the
one-execution-program architecture, both backends, the config matrix — is **done** (census §8). The
residual is the **(A) quality** breadth: per-feature error thresholds (PSNR/SSIM/FLIP of
shadows/GI/materials/AA), the XR/foveation/stereo/dyn-res matrix, and shipping/compat. Does that (A)
quality work belong *inside* CEIR-35, or does CEIR-35 close on the substrate floor with (A) routed to
the feature bands that own each feature?

> ⚠ Honesty note carried from census §9: the "PQP-0..4 / PSNR-SSIM-FLIP" breakdown is the census's
> *mapping* of your phrase "whole-engine PQP" onto the mission §PR-7 text — **not a verbatim
> acceptance-criteria list you gave**. Your recorded words are "finish CEIR fully, gold-standard, no
> gaps" + the AskUserQuestion selection "whole-engine PQP." That is exactly why this is a decision:
> inventing the thresholds/corpus/target-matrix autonomously = inventing acceptance criteria.

**The forks.**

- **A1 — pull specific quality gates into CEIR-35 now, by your explicit list.** You name which
  feature-quality metrics + thresholds + platform targets belong in the CEIR loop; each becomes a gated
  slice `35g…` with a reference-image corpus. **Unblocks:** a single qualified "CEIR-35 = whole-engine
  quality" verdict. **Cost:** materially enlarges the band; needs your criteria + the Q7 bench-design
  steer first; some gates (XR, console/mobile shipping) depend on hardware/targets not yet committed.
- **A2 — close CEIR-35 on the (B) breadth + spine floor; route (A) quality to the feature bands with
  triggers.** The CEIR *execution system* is production-qualified (spine §2/§7 + breadth §8);
  feature-quality qualification lands with each feature as it matures on the finished substrate.
  **Unblocks:** 35z close now (after Q7); each quality gate becomes a named, triggered obligation on its
  owning band rather than an open-ended CEIR residual.

**My lean: A2.** The CEIR loop's job is the *substrate*; §8 shows it is production-grade at engine
breadth, and (A)'s acceptance criteria genuinely live with the features (a shadow-PSNR gate belongs to
the rendering track that ships shadows, not to the IR substrate). **But this defines what "CEIR done"
MEANS, is irreversible-shaped, and rests on a paraphrased ruling — so it is surfaced, not decided.**
⛔ Until you rule, `context.md` must not read "whole-engine PQP done" on (B) alone.

**A2 routing table (pre-drafted — if you pick A2, this is the mechanical half; confirm/edit the bands).**
Each (A) quality dimension → the band that owns it → the trigger that opens the gate → the concrete
roadmap anchor. Owning-band labels are the census §9 set; roadmap anchors are cited where confirmed.

| (A) quality dimension | Owning band (census §9) | Trigger to open the gate | Roadmap anchor |
|---|---|---|---|
| Rendering-feature error metrics — shadows / GI / AA (PSNR/SSIM/FLIP vs reference corpus) | **Track A (rendering)** | that feature reaches its shipping slice AND a reference-image corpus + threshold is defined | Track A rendering bands (post-CEIR) |
| Material-appearance quality (BRDF/BTDF/layering error vs MaterialX/Mitsuba reference) | **MAT** (material track) | material feature ships + reference render defined | Phase 2.8 material track continuation |
| XR / foveation / stereo / dynamic-resolution feature matrix | **VGE / ARG** (per census) | an XR hardware target is committed | **Phase 6 platform expansion — VR/AR OpenXR (ADR-0077)** |
| Shipping / compat — driver matrix, console/mobile/web feature-level fallbacks | **VGE / ARG** (per census) | a ship-candidate target list is committed | **Phase 6 platform expansion (PS5/Xbox/Switch, iOS/Android, WebGPU)** |
| Editor-surfaced quality (the program-editor's own render/inspect fidelity) | **I2D-PQ** | CR-D007 / I2D-9 exists to consume it | **Phase 7 editor / I2D-9 — ties to Decision C** |

> The XR/shipping rows route to **Phase 6** (confirmed: ROADMAP line 62 + 370, ADR-0077). The editor row
> ties to **Decision C** (the D7E/I2D-9 half). The "VGE / ARG" band labels are the census's own shorthand;
> if those are not the bands you intend for XR/shipping, name the destination and I re-point the row —
> the routing *shape* (dimension → band → trigger → anchor) is what A2 commits to.

---

## Q7 — the CEIR-35 perf-board bench design (the one held slice)

**The question in one sentence.** 35d-Q7 (perf boards for the CEIR execution paths) is **held for your
bench-design steer** — you hold strong, recorded opinions (⛔ **all-peers / no-cherry-pick / full-crush**;
references are the floor; every measured board → `docs/bench/` at measurement time), and a perf board
designed without them would violate the bench policy.

**What I need from you.** For the CEIR spine/execution perf boards: (1) the **peer set** to bench against
(which gold-standard references define the floor for each path — e.g. cuBLAS/CUTLASS for GEMM,
vendor path-tracers for RT, etc.); (2) whether the boards measure the **substrate paths only** (dispatch/
graph-replay/plan-execute overhead) or **whole-feature end-to-end**; (3) the **target hardware** the board
is measured on. **Unblocks:** 35d-Q7 → the last CEIR-35 spine slice → **35z close** (Q7 is the only held
slice under both A1 and A2). Independent of the A-fork: perf boards are owed either way.

---

## Decision C — CEIR-33 (D7E, the universal program editor): does it close at I2D-9, or on the domain contracts?

**The question in one sentence.** CEIR-33/D7E splits into **33a domain contracts** (authorable schemas +
round-trip/cook + hot-reload for each editor domain — **drivable, and graded now: 4 DONE / 1 PARTIAL /
2 GAP-until-consumed**, census §2) and **33b the I2D-9 flagship widgets** (the UI half, which rides the
CR-D007 shell). The 33b half is gated on **ADR-0107, already pending your review** — this is *not a new
ask*. Does CEIR-33 close when I2D-9 closes, or does it close on the domain-contract deliverable with the
widgets tracked as I2D-9?

**The forks.**

- **C1 — accept ADR-0107 → the I2D band proceeds** (I2D-1 Canvas → I2D-2 Text → I2D-3 UiWorld → I2D-4
  CR-D007 → … → I2D-9 flagship widgets). CEIR-33/D7E **closes when I2D-9 closes**; the CEIR-L7 authoring
  ceiling (§174 manifest) unlocks with CR-D007. **Unblocks:** the full editor programme + CEIR-L7/L8.
  **Cost:** a large multi-band UI programme, not a CEIR slice — CEIR-33 stays open across it.
- **C2 — rule that CEIR-33's deliverable is the domain contracts + the L6 bar** (33a), and re-scope the
  widgets explicitly to I2D-9. CEIR-33 then closes on the contract inventory (§2) reaching DONE for the
  shipping domains; the widgets are tracked as I2D-9. **Unblocks:** CEIR-33 closes now; the editor UI
  becomes an I2D-band obligation on its own timeline.

**My lean: C2** — it is D8's own text ("D7E therefore = the domain contracts those widgets satisfy"), and
under **either** fork the drivable work is identical and already done (33a: 4 DONE / 1 PARTIAL /
2 GAP-until-consumed). C is purely the *closure* question. It rides the **already-pending ADR-0107
review** — ruling on ADR-0107 effectively rules C.

> The GAP domains (geometry-graph editor, capture/profiling contract) are **GAP-until-consumed by the
> I2D-9 widget**, not GAP-as-debt: defining a `ceir.geom` graph schema before the widget that consumes it
> exists would be speculative invention (⛔ search-engine-before-building; everything-is-an-authorable-asset
> is a *pull* from a consumer). So neither fork owes them now.

---

## Cross-cutting — record, don't decide (surfaced for completeness)

- **ceir-0g deferred schema=3 + §PR-4 taxonomy gap.** All 11 CEIR-native manifest rows carry
  `raf_level="n/a"` by construction, which sharpens two *already-deferred* questions: (i) retiring
  `raf_level` for converging classes (schema=3), and (ii) the missing native taxonomy class for
  compiler-transforms/frontends (filed as "B-machinery"). **Same posture as Q10 was: record, user-gated,
  not a blind fix.** No loop action.
- **The §174 CEIR-L7 ceiling stays coupled to Decision C.** L7 = CR-D007 authoring; it unlocks with the
  C1 path. This is honestly recorded in the manifest ceiling note, not a pending fill.

## The closure sequence

1. **Q7 steer** → 35d-Q7 perf boards → the last spine slice.
2. **Decision A** (A1 pull-in-by-list · **A2 close-and-route** — lean A2; table above pre-drafted).
3. **Decision C** rides the **pending ADR-0107 review** (C1 accept · **C2 contracts+L6** — lean C2).
4. → **35z close** (A + Q7) ; CEIR-33 closes on the C-fork.

> No irreversible action is taken here. Agents do not commit; the uncommitted working tree awaits your
> batch commit (Conventional Commits, **no AI co-author trailer**, per project policy).
