# Design contracts

<!-- doc-role: navigation -->
> Navigation; no independent live queue. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Current work/status: [ROADMAP](../ROADMAP.md). This is a reference lookup, never a competing queue.
Search a title/ID, open its document, and respect its Proposed/Accepted/supersession notes.
Older index commentary is [preserved](../archive/2026-09-12-orientation-history.md#docs-design-readme).

Contracts retain full scope. Before implementation, refresh the source reuse census, increments, failure cases
and verification evidence in the linked note; its live status belongs only to the master row.

| Reference | Purpose |
|---|---|
| [Developer workflow](developer-workflow.md) | Conservative affected-target/CTest planning, scoped frontend and exact content evidence |
| [Visual Studio configurations](visual-studio-configurations.md) | Full CMake preset selector, eight native MSVC profiles and configuration-aware synchronization gates |
| [Project structure synchronization](project-structure-sync.md) | Transaction, IDE/CMake round-trip, conflict/recovery and physical path migration contract |
| [Repository layout](repository-layout.md) | Physical/IDE families, hygiene, preservation and build/CI verification |
| [DX12 workload repair](dx12-workload-repair.md) | Validation readiness, resource states, descriptor lifetime/exhaustion and actual authored-consumer qualification |
| [System qualification contract](system-quality-contract.md) | Cross-platform, ownership, correctness, security, agent access and complete document/session close-out |
| [ceir-0a-execution-path-inventory](ceir-0a-execution-path-inventory.md) | CEIR-0a — Execution-path inventory (FROM CODE) |
| [ceir-0e-chir-0-language-design-note](ceir-0e-chir-0-language-design-note.md) | CEIR-0e — CHIR-0: the Cerid high-level language design note (DESIGN-ONLY) |
| [ceir-0g-maturity-and-manifest](ceir-0g-maturity-and-manifest.md) | CEIR-0g — One maturity model + the §174 machine-readable manifest |
| [ceir-0h-migration-and-deletion-tables](ceir-0h-migration-and-deletion-tables.md) | CEIR-0h — Migration + explicit DELETION tables |
| [ceir-0z-close-report-and-sizing](ceir-0z-close-report-and-sizing.md) | CEIR-0z — The §184 close report + honest CEIR-1…13 sizing |
| [ceir-10a-hot-reload-and-state-migration](ceir-10a-hot-reload-and-state-migration.md) | CEIR-10a — Hot-reload lifecycle + state migration — DESIGN NOTE |
| [ceir-10b-execution-plan-cache](ceir-10b-execution-plan-cache.md) | CEIR-10b — Execution-plan cache — DESIGN NOTE |
| [ceir-11a-reference-executor](ceir-11a-reference-executor.md) | CEIR-11a — Reference executor (full host subset) + async-host + step hooks — DESIGN NOTE |
| [ceir-11b-compiled-execution-plan](ceir-11b-compiled-execution-plan.md) | CEIR-11b — Plan compiler + `CompiledExecutionPlan` + the differential harness — DESIGN NOTE |
| [ceir-13z-execution-proof](ceir-13z-execution-proof.md) | CEIR-13z — the §129 execution proof (design note, stage 1) |
| [ceir-14-render-dialect](ceir-14-render-dialect.md) | CEIR-14 — `ceir.render` dialect (design note / design-lock) |
| [ceir-14z-render-execution-proof](ceir-14z-render-execution-proof.md) | CEIR-14z — the render DEVICE pixel proof (14z-0 decision packet) |
| [ceir-15-framegraph-unification](ceir-15-framegraph-unification.md) | CEIR-15 — FrameGraph unification: the `ceir.frame` dialect (15-0 decision packet) |
| [ceir-16-executor-migration](ceir-16-executor-migration.md) | CEIR-16 — Executor migration (decision packet) |
| [ceir-17-scene-bridge](ceir-17-scene-bridge.md) | CEIR-17 — Scene / ECS / geometry bridge (design + STATUS ledger) |
| [ceir-33-d7e-orientation-census](ceir-33-d7e-orientation-census.md) | CEIR-33 (D7E — the universal program editor) — orientation census |
| [ceir-34-legacy-deletion-census](ceir-34-legacy-deletion-census.md) | CEIR-34 — Legacy-deletion census (band-open, GATE-re-verifies) |
| [ceir-35-production-qualification-census](ceir-35-production-qualification-census.md) | CEIR-35 — Production-qualification census (band-open orientation) |
| [ceir-35z-user-decisions](ceir-35z-user-decisions.md) | CEIR-35z — the user-gated decisions (one place to cross the wall) |
| [hesap_fft_generated_codelets](hesap_fft_generated_codelets.md) | Hesap FFT — Generated Codelet / Planner Project (design) |
| [raf-0-rendering-foundation-design](raf-0-rendering-foundation-design.md) | RAF-0 — Gold-Standard Asset-Driven Rendering Foundation: DESIGN SPEC |
| [raf12-verb-relocation](raf12-verb-relocation.md) | raf12-verb-relocation |
| [ren-2-rtt-and-material-textures](ren-2-rtt-and-material-textures.md) | REN-2 design — render-to-texture transients + sampled material textures (D-007 row 99) |
| [ren-3-1-depth-rtt-transients](ren-3-1-depth-rtt-transients.md) | REN-3.1 — RTT **DEPTH** transients (the shadow-map substrate) |
| [ren-3-lighting-shadow-pipeline](ren-3-lighting-shadow-pipeline.md) | REN-3 design — the gold LIGHTING · SHADOW · SKY · ANTI-ALIASING device pipeline (D-007 row 100) |
| [ren-36-authorable-frame-graph](ren-36-authorable-frame-graph.md) | REN-36 — THE AUTHORABLE FRAME GRAPH: render passes, pipelines and whole rendering architectures as ASSETS |
| [ren-37-material-technique-composition](ren-37-material-technique-composition.md) | REN-37 design — MATERIAL × TECHNIQUE composition: how an authored frame graph reaches into the fragment shader |
| [ren-41-stage4-nanite-cluster-lod](ren-41-stage4-nanite-cluster-lod.md) | REN-41 Stage 4 — Nanite cluster-LOD renderer integration (design contract) |
| [ren-band-reuse-audit](ren-band-reuse-audit.md) | REN band — the REUSE AUDIT (all 35 rows) |
| [renderer-ui-execution-contract](renderer-ui-execution-contract.md) | Renderer, crd-ui and CR-D007 execution contract |
| [rendering-ui-contracts](rendering-ui-contracts.md) | Inherited renderer and UI requirements |
