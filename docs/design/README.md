# `docs/design/` — per-slice IMPLEMENTATION SPECS

> **What this directory is for.** A design doc here is the **implementation contract for ONE slice**: what
> already exists (so it is reused, not rebuilt), what is genuinely missing, the sequenced increments, the gate
> for each, the named risks, and the explicit non-goals. It sits between the **row** (the one-line contract in
> a phase/detour table) and the **session log** (what actually happened).
>
> **How to find the doc for a slice you have been asked to implement:** the slice's row in its phase or detour
> table links it BY PATH. Start at `context.md` → the active phase/detour → the row → this directory. The index
> below is the reverse lookup.

## Conventions

- **One file per slice**, named `<slice-id>-<slug>.md` (e.g. `ren-3-lighting-shadow-pipeline.md`).
- **The row is the pointer.** Whenever a spec is written, the slice's row MUST be edited to link it — a design
  doc that nothing points at is invisible to the next agent, which is the exact failure this index exists to
  prevent (found 2026-07-25: `docs/design/` was not referenced from `docs/README.md` at all, so the canonical
  reading order never reached it).
- **A REUSE AUDIT is mandatory before the increments.** Grep the engine first (SANITY #8) and state, per gap,
  whether it is *wiring* or *new work*, with file:line evidence. The REN-3 spec's sky section is the worked
  example: what read as "new work" turned out to be four LUT kernels already dispatching oracle-green on
  Vulkan, which shrank the increment substantially.
- **Every increment carries its gate**, and the whole slice carries its acceptance criteria + named
  non-goals, so an omission is a decision rather than drift.
- **Specs are living until their slice closes**, then they are historical — the session log supersedes them as
  the record of what shipped. Do not retro-edit a closed spec; write the divergence in the session log.

## Index

| slice | doc | status |
|---|---|---|
| **CEIR-0a** | [ceir-0a-execution-path-inventory.md](ceir-0a-execution-path-inventory.md) — the from-code inventory of every execution-program representation (executors · frame-graph runtime · scene_renderer orchestration + hand-list · IComputeContext · cookers · .crdr · draw · media), classified composite-vs-atomic. Headline: RAF already did the split — CEIR is a promotion, not a rewrite | ✅ complete 2026-08-07 |
| **CEIR-0e** | [ceir-0e-chir-0-language-design-note.md](ceir-0e-chir-0-language-design-note.md) — the CHIR high-level language design NOTE (design-only, decides nothing; the binding decisions are a CEIR-29 ADR). §98 feature set · ownership-model options weighed (leaning: values + generational handles + arenas + state stores + light borrows, NOT a full borrow checker) · syntax sketches · the text/visual projection model | ✅ accepted (direction) 2026-08-07; binding decisions deferred to CEIR-29 |
| **CEIR-0g** | [ceir-0g-maturity-and-manifest.md](ceir-0g-maturity-and-manifest.md) — reconciles the two maturity models (post-RAF L0–L7 vs CEIR L0–L8) into ONE forward model + a two-axis transition (`raf_level` = today's reality, `ceir_level` = the forward track, both honest); + the §174 manifest field set (adds `providers` + `determinism_tier`) + the registry-migration plan | ✅ accepted 2026-08-08 |
| **CEIR-0h** | [ceir-0h-migration-and-deletion-tables.md](ceir-0h-migration-and-deletion-tables.md) — the migration + DELETION ledger built from CEIR-0a: what promotes vs deletes, every deletion with its parity gate named FIRST (frame-path F1–3 @ CEIR-12 · orchestration E1–5 @ CEIR-13 · residual special-cases R1–2 @ CEIR-11 · §PR-3 supersession @ 0f). CEIR-31 executes it verbatim | ✅ accepted 2026-08-08 |
| **CEIR-0z** | [ceir-0z-close-report-and-sizing.md](ceir-0z-close-report-and-sizing.md) — the CEIR-0 close: the mission §184 fifteen-item report (each answered from 0a–0h evidence) + honest DERIVED sizing (CEIR-1…13 ≈ 34–55 KLOC with in-tree anchors + per-band confidence; ~4–8 mo dark period, very-low confidence) + the 5 unresolved-design-questions docket | ✅ accepted 2026-08-08 |
| **CEIR-10a** | [ceir-10a-hot-reload-and-state-migration.md](ceir-10a-hot-reload-and-state-migration.md) — hot-reload + state-migration slice spec (ADR-0120) | ✅ closed |
| **CEIR-10b** | [ceir-10b-execution-plan-cache.md](ceir-10b-execution-plan-cache.md) — execution-plan cache (ADR-0121) | ✅ closed |
| **CEIR-11a** | [ceir-11a-reference-executor.md](ceir-11a-reference-executor.md) — the reference executor, full host subset (ADR-0122) | ✅ closed |
| **CEIR-11b** | [ceir-11b-compiled-execution-plan.md](ceir-11b-compiled-execution-plan.md) — compiled execution plan (ADR-0123) | ✅ closed |
| **CEIR-13z** | [ceir-13z-execution-proof.md](ceir-13z-execution-proof.md) — the §129 execution proof (the cornerstone flip) | ✅ closed |
| **CEIR-14** | [ceir-14-render-dialect.md](ceir-14-render-dialect.md) — the `ceir.render`/`ceir.frame` dialect (ADR-0127) | ✅ closed |
| **CEIR-14z** | [ceir-14z-render-execution-proof.md](ceir-14z-render-execution-proof.md) — render execution proof | ✅ closed |
| **CEIR-15** | [ceir-15-framegraph-unification.md](ceir-15-framegraph-unification.md) — frame-graph unification | ✅ closed |
| **CEIR-16** | [ceir-16-executor-migration.md](ceir-16-executor-migration.md) — executor migration | ✅ closed |
| **CEIR-17** | [ceir-17-scene-bridge.md](ceir-17-scene-bridge.md) — the scene bridge (`scene_renderer` → authored assets) | ✅ closed |
| RAF-0 | [raf-0-rendering-foundation-design.md](raf-0-rendering-foundation-design.md) — the RAF rendering foundation (ADR-0106) | ✅ closed |
| RAF-12 | [raf12-verb-relocation.md](raf12-verb-relocation.md) — RAF-12 verb relocation | ✅ closed |
| **CEIR-18…25** | _no separate spec files_ — bands 18-25 (`ceir.rt`/`work`/`shape`/`tensor`/`layout`/`linalg`/`quant`/`sparse`/`ml`/`autodiff` + provider partitioning) were design-locked INLINE in the tracker's per-band **BAND-OPEN LOCK** blocks (`docs/detours/D-007-ceir-tracker.md`) + the ADRs, not as standalone specs | 📋 see tracker |
| REN-2 | [ren-2-rtt-and-material-textures.md](ren-2-rtt-and-material-textures.md) — RTT transients + sampled material textures | ✅ closed 2026-07-25 |
| REN-3 | [ren-3-lighting-shadow-pipeline.md](ren-3-lighting-shadow-pipeline.md) — lighting · shadow · procedural sky · full AA, **sandbox-visible** | ◼ superseded — the pre-RAF REN band was superseded by RAF/post-RAF (2026-08-07 trim); the shipped parts (CSM atlas, sky) live in the `forward_csm` frames + session logs |
| REN-3.1 | [ren-3-1-depth-rtt-transients.md](ren-3-1-depth-rtt-transients.md) — RTT **depth** transients + `draw_storage_depth_only` (the shadow-map substrate) | ✅ closed 2026-07-25 (gated both backends + bench) |
| **REN-36** | [ren-36-authorable-frame-graph.md](ren-36-authorable-frame-graph.md) — ⭐ **render passes, pipelines and whole rendering ARCHITECTURES as authorable assets, API-agnostic** (user-declared MUST) | ✅ shipped — the authoring stack landed via REN-36/37/38 (2026-07-25…27) and became the RAF foundation |
| REN band | [ren-band-reuse-audit.md](ren-band-reuse-audit.md) — pass-1 audit of all 35 rows. **Read its method warning**: pass 1 mostly re-derived what the rows already say and was wrong twice; the rows carry their own reuse notes | 📋 reference |
| hesap-fft | [hesap_fft_generated_codelets.md](hesap_fft_generated_codelets.md) — the generated FFT codelet scheme | ✅ shipped |
| REN-37 | [ren-37-material-technique-composition.md](ren-37-material-technique-composition.md) — **MATERIAL x TECHNIQUE composition**: how an authored frame graph reaches into the FRAGMENT SHADER. Three authored layers (material=surface / technique=lighting / frame graph=schedule), the binding contract, ubershader-vs-variants resolved by our IR, and the variant collapse lowering gives for free | ✅ shipped 2026-07-27 (REN-37 — material × technique composition live; `.crdm`/`.crdt` authored programs) |
| **REN-41 Stage 4** | [ren-41-stage4-nanite-cluster-lod.md](ren-41-stage4-nanite-cluster-lod.md) — **Nanite cluster-LOD renderer integration**: wire the CLOSED 40-I cluster-DAG (cook + BVH select + unpack) into the renderer — a GPU `cluster_select` compute kernel, REAL cluster task+mesh `.crdv` programs, packed-buffer upload + a `MeshRenderer` route flag, authored `cluster_select`→`mesh_draw` passes. Reuse audit + 5 de-risked increments (mesh-body spike first) + risks/non-goals | ◧ folded into the post-RAF **VGE** band (40-I cluster-DAG + REN-41 S4-0 shipped; the renderer integration continues as VGE in D-007) |
