# Cerid — current pointer

<!-- doc-role: pointer -->
> Current-work pointer. Current work: [ROADMAP](docs/ROADMAP.md); current rules: [AGENTS](AGENTS.md).

<!-- current-slice: SIMD-DISPATCH -->
**Current work:** [SIMD-DISPATCH](docs/ROADMAP.md#slice-simd-dispatch) (Partial): the sse2 boards' findings
(the nn, Gamma and Student-t regressions), then jobs retune and fft f32 against MKL.
[DIAG.11b](docs/ROADMAP.md#slice-diag.11b): user re-record (8). [DIAG.11c](docs/ROADMAP.md#slice-diag.11c),
[DIAG.12](docs/ROADMAP.md#slice-diag.12): final complete-tier run.
Local routes: cached trees, WSL mirror, mold links, bounded waits ([build performance](docs/design/build-performance.md)).
[Contract](docs/design/runtime-diagnostics.md); [runbooks](docs/design/diagnostic-runbooks.md).
**Only tracker:** [ROADMAP](docs/ROADMAP.md). Rules: [AGENTS](AGENTS.md); lessons: [MEMORY](MEMORY.md).

## Product direction

Order: the accepted [delivery plan](docs/design/delivery-plan.md) and
[ADR-0129](docs/decisions/0129-renderer-ui-editor-delivery-order.md) with its amendments: backbones before their
consumers, then the renderer library, crd-ui and **CR-D007**, then the notebook (a CR-D007 workspace plus a host).
CHIR authors behaviour, CEIR executes, CKIR expresses device programs. Every shipping algorithm is an editable asset.

## Evidence and boundaries

Last engine milestone: [CEIR-35 close](docs/sessions/2026-09-11-ceir-35z-band-close.md); execution foundation only.
Handoffs: the current row's latest session in its ROADMAP evidence.
IDE: [configurations](docs/design/visual-studio-configurations.md); [sync/recovery](docs/design/project-structure-sync.md).
Entry: [START_HERE](START_HERE.md); routes: [CONTRIBUTING](docs/CONTRIBUTING.md).
Collaboration/multiplayer contracts: [ADR-0130](docs/decisions/0130-system-qualification-and-agent-driven-products.md).

Process ([AGENTS](AGENTS.md)): one serial workflow takes rows in order, reads CI first, tests only changes locally
and pushes each item; parallel agents run only by user order (the CI rows since 2026-10-30) on their own branches.
On 2026-10-05 the user accepted all open recommendations
([decisions](docs/sessions/2026-10-05-user-decisions.md)): lavapipe CI, 7a lifetime/pass, TSan lane, WARP fault.
Pending gates remain in ROADMAP; the [audit](docs/sessions/2026-09-14-infrastructure-audit.md) maps child evidence.
ADR-0107 and RAH-0 reviews remain gates. No invented approval or test.
