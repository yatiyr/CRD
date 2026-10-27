# ADR-0129 — Renderer, UI, CR-D007 and notebook delivery order

<!-- doc-role: decision -->
> Decision record; read status and supersession notes. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**Status:** Accepted — explicit user decisions, 2026-09-12.
**Tags:** [roadmap] [rendering] [ui] [editor] [hesap] [platform]

## Decision

Complete the entire retained renderer feature library before starting UI implementation. This includes renderer
hardening, all retained rendering families, virtual geometry, hybrid GI, production path tracing and OFF-1…9.
Compute/neural prerequisites required by those renderers are brought forward explicitly; unrelated numerical or
media programmes are not silently started.

Then complete the Cerid-owned UI and **CR-D007**, the game and engineering application editor. Bootstrap CR-D007
at I2D-4 within that UI programme, and build the remaining UI/editor widgets inside it. The editor includes real
scene/viewport/gizmo/asset/program authoring and standalone game publishing. ImGui remains debug/recovery only.

Then finish hesap GPU and the MATLAB-like notebook. The notebook is a reusable document workspace in CR-D007 and
has a thin standalone host using the same modules. Media, audio/plugins, later applications and other retained
D-007/engine programmes follow. Geometry, physics and unrelated implementation modules remain untouched during
this documentation audit; their work is retained for its own authorized slices.

Windows and Linux qualify first. macOS and web are explicit follow-on releases, requiring actual application/runtime
qualification in addition to shader emitters.

One [master table](../ROADMAP.md#master-table) owns all live slices/subslices. Redundant documents may be removed
after their unique content and references are preserved. Detailed contracts, ADRs, benchmark evidence, recipes and
historical records remain available as references; they are not competing trackers.

## Consequences

**2026-09-14 amendment:** the user adds the runtime diagnostic foundation **DIAG** immediately after REPO.DEV,
before retained repository closure and renderer implementation. [ADR-0133](0133-runtime-diagnostics-and-instrumentation.md)
defines this prerequisite; renderer → UI/editor → notebook product order and all review gates remain intact.

Renderer runtime features and text-asset editing/reload qualify before UI. Their visual CR-D007 authoring acceptance
necessarily follows I2D-9; parent rows remain open until both stages pass. This is a dependency distinction, not a
removal of the editor requirement.

Earlier notebook-first, eylem-before-UI and parallel-renderer/UI scheduling clauses are superseded by this order.
The complete inherited scope is retained. This ADR does **not** accept the whole Proposed ADR-0107, approve an
unreviewed breaking canonical model, reinstate the completed CEIR autonomous loop or claim new runtime tests.

**2026-10-27 amendment (user decision, [delivery plan](../design/delivery-plan.md)):** backbones precede their
consumers. After the RAH redesign and the computational geometry backbone, the table runs foundation hardening and
the ARM64 port; the CHIR language, reflection, command, scene/ECS/world, document, scripting, extension and agent
backbone; hesap CPU and GPU, the GPU compute programs and the machine-learning core; SDF, B-rep, analysis meshing and
the FE/FV cores; the animation core; and the physics engine EYLEM with its numeric qualification, all before the
renderer runtime library. The sentence above that supersedes "eylem-before-UI" clauses no longer applies: physics
now precedes the renderer and the UI because the renderer consumes it (GVA-3.runtime qualifies cloth and soft-body
input from a real producer). The renderer runtime library still completes before any UI implementation
(RENDERER-RUNTIME), CR-D007 still precedes the notebook, and the notebook still follows as a CR-D007 workspace.
Media, audio, the macOS and web releases and the product verticals follow in the plan's order. A row that needs
hardware or SDK access the project does not have qualifies on emulators, simulators and software adapters at its
place, and its real-hardware half runs at the end of the table. No row is Later.

## References

- [Execution contract](../design/renderer-ui-execution-contract.md)
- [Inherited requirements](../design/rendering-ui-contracts.md)
- [System audit and confirmed decisions](../research/2026-09-12-system-audit.md)
- [ADR-0107 — UI architecture proposal](0107-ui-2d-architecture.md)
- [ADR-0109 — CHIR/CEIR/CKIR ownership](0109-ceir-chir-ckir-ownership-and-module-placement.md)
