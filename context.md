# Cerid — current pointer

<!-- doc-role: pointer -->
> Current-work pointer. Current work: [ROADMAP](docs/ROADMAP.md); current rules: [AGENTS](AGENTS.md).

<!-- current-slice: DIAG.4b -->
**Current work:** [DIAG.4b](docs/ROADMAP.md#slice-diag.4b), first unfinished row: its GenMC weak-memory model has
never been run (pin GenMC, run it on WSL; the user decides lane vs recorded run). DIAG.3a-4a closed on complete runs
of `6bd1ba59`; 4c-6b and 7a wait in order, 6c needs its script suites in CI
([audit](docs/sessions/2026-10-06-needs-ci-audit.md#rows-closed-on-6bd1ba59-2026-10-09)).
[Implementation contract](docs/design/runtime-diagnostics.md).
**Only tracker:** [ROADMAP](docs/ROADMAP.md). Rules: [AGENTS](AGENTS.md); lessons: [MEMORY](MEMORY.md).

## Product direction

Entire retained real-time/offline renderer → complete crd-ui and **CR-D007** → hesap GPU/notebook → media and
preserved programmes. Windows/Linux first; macOS/web follow. Notebook = a CR-D007 workspace plus a host.
[ADR-0129](docs/decisions/0129-renderer-ui-editor-delivery-order.md) owns that order; the master table owns its state.
CHIR authors behaviour, CEIR executes, CKIR expresses device programs. Every shipping algorithm is an editable asset.

## Evidence and boundaries

Last engine milestone: [CEIR-35 close](docs/sessions/2026-09-11-ceir-35z-band-close.md); execution foundation only.
Handoff: [nightly CI repairs](docs/sessions/2026-10-04-nightly-ci-repairs.md).
IDE: [configurations](docs/design/visual-studio-configurations.md); [sync/recovery](docs/design/project-structure-sync.md).
Entry: [START_HERE](START_HERE.md); routes: [CONTRIBUTING](docs/CONTRIBUTING.md).
Collaboration/multiplayer contracts: [ADR-0130](docs/decisions/0130-system-qualification-and-agent-driven-products.md).

User assignment 2026-10-04: work serially, no parallel agents; make CI fully green first. Changes go to CI without
local whole-tree builds/tests; read CI, repair, repeat. On 2026-10-05 the user accepted all open recommendations
([decisions](docs/sessions/2026-10-05-user-decisions.md)): lavapipe CI, 7a lifetime/pass, TSan lane, WARP fault.
Pending gates remain in ROADMAP; the [audit](docs/sessions/2026-09-14-infrastructure-audit.md) maps child evidence.
ADR-0107 and RAH-0 reviews remain gates. Existing hardware; serial agent pushes per user (no AI notes); no invented
approval or test.
